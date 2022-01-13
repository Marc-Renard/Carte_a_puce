#include <avr/io.h>
#include <stdint.h>
#include <avr/eeprom.h>
#include <avr/pgmspace.h>
#include <stdarg.h>

//---------------------
// 
// Programme bourse.c
// 
//---------------------
//====================================================================================================
/* Pour gérer les transactions, il a fallu créer deux fonctions pour harmoniser les écritures dans l'EEPROM:
   l'engagement et la validation. Pour engager, on doit utiliser une structure permettant de gérer les données à valider */

/*Il faut aussi gérer l'état de cette structure, càd que si elle est dans un état viable et qu'il y'a des données à valider
 une variable d'état doit le notifier */

/* Si au démarrage de la carte, cette variable indique que la structure est pleine, cela signifie que la carte
   a pu engager les données dans la structure mais a été coupée lors de la validation; on doit donc relander la procédure de validation */


//======================================================================================
// déclaration des fonctions d'entrée/sortie
// écrites en assembleur dans le fichier io.s
void sendbytet0(uint8_t b);
uint8_t recbytet0(void);
void valide();

// variables globales en static ram
uint8_t cla, ins, p1, p2, p3;  // header de commande
uint8_t sw1, sw2;              // status word

#define MAXI 128	// taille maxi des données lues
uint8_t data[MAXI];	// buffer pour les données introduites
int taille;		// taille des données introduites


#define size_atr 6
const char atr_str[size_atr] PROGMEM = "bourse"; // Déclaration et initialisation du nom du programme, ce qui permettra de l'envoyer lors des resets



//########################################################################################################################
//############################################           TEA              ################################################
//########################################################################################################################

// chiffrement
// clair[2] : clair 64 bits
// crypto[2] : cryptogramme calculé 64 bits
// k[4] : clé 128 bits
//clé utilisée: 54 BA 93 54 D5 67 6C E6 4C A7 CF 93 83 71 B9 30
void tea_chiffre(uint32_t * clair,uint32_t * crypto, uint32_t * k)
{
    uint32_t    y=clair[0],
                z=clair[1],
                sum=0;
    int i;
    for (i=0;i<32;i++)
    {
        sum += 0x9E3779B9L;
        y += ((z << 4)+k[0]) ^ (z+sum) ^ ((z >> 5)+k[1]);
        z += ((y << 4)+k[2]) ^ (y+sum) ^ ((y >> 5)+k[3]);
    }
    crypto[0]=y; crypto[1]=z;
}

// déchiffrement
// crypto[2] : cryptogramme
// clair[2] : clair calculé
// k[4] : clé 128 bits
void tea_dechiffre(uint32_t* crypto ,uint32_t* clair, uint32_t*k)
{
    uint32_t    y=crypto[0],
                z=crypto[1],
                sum=0xC6EF3720L;
    int i;
    for (i=0;i<32;i++)
    {
        z -= ((y << 4)+k[2]) ^ (y+sum) ^ ((y >> 5)+k[3]);
        y -= ((z << 4)+k[0]) ^ (z+sum) ^ ((z >> 5)+k[1]);
        sum -= 0x9E3779B9L;
    }
    clair[0]=y; clair[1]=z;
}




//########################################################################################################################
//############################################           ATR              ################################################
//########################################################################################################################
void atr()
{
	int i;
    	sendbytet0(0x3b); 	// définition du protocole
    	sendbytet0(size_atr);		// nombre d'octets d'historique
    	for (i=0;i<size_atr;i++)		// Boucle d'envoi des octets d'historique
    	{
        	sendbytet0(pgm_read_byte(atr_str+i));
    	}
    	valide();
}

#define size_ver 4
const char ver_str[size_ver] PROGMEM = "1.00"; // Déclaration et initialisation de la version en mêmoire programme

//########################################################################################################################
//##########################################           VERSION              ##############################################
//########################################################################################################################
void version()
{
    	int i;
    	// vérification de la taille
    	if (p3!=size_ver)
    	{
        	sw1=0x6c;	// taille incorrecte
        	sw2=size_ver;	// taille attendue
        	return;
    	}
	sendbytet0(ins);	// acquittement
	// émission des données
	for(i=0;i<p3;i++)
    	{
        	sendbytet0(pgm_read_byte(ver_str+i));
        	//pgm_read_byte va lire dans la mémoire programme la variable ver_str
		//et sendbytet0 envoie ce qui est lu soit 1.00
    	}
    	sw1=0x90;
}

// transactions
//-------------

// nombre maximal d'opérations par transaction
#define max_ope		3
// taille maximale totale des données échangées lors d'une transaction
#define max_data	64
// définition de l'état du buffer -- plein est une valeur aléatoire
typedef enum{vide=0, plein=0x1c} state_t;
// la variable buffer de transaction mémorisée en eeprom
struct
{
	state_t state;			// etat
	uint8_t nb_ope;			// nombre d'opération dans la transaction
	uint8_t tt[max_ope];		// table des tailles des transferts
	uint8_t*p_dst[max_ope];		// table des adresses de destination des transferts
	uint8_t buffer[max_data];	// données à transférer
}
ee_trans EEMEM={vide}; // l'état doit être initialisé à "vide"

//########################################################################################################################
//########################################           VALIDATION              #############################################
//########################################################################################################################
void valide()
{
	state_t e;		// état
	uint8_t nb_ope;		// nombre d'opérations dans la transaction
	uint8_t*p_src, *p_dst;	// pointeurs sources et destination
	uint8_t i,j;
	uint8_t tt;		// taille des données à transférer

	// lecture de l'état du buffer
	e=eeprom_read_byte((uint8_t*)&ee_trans.state);
    
	// s'il y a quelque chose dans le buffer, transférer les données aux destinations
	if (e==plein)
	{
		
		nb_ope=eeprom_read_byte(&ee_trans.nb_ope);   // récupération du nombre d'opérations
		p_src=ee_trans.buffer; //initialisation du pointeur vers la source
		
		for (i=0;i<nb_ope;i++) //Cette boucle va effectuer les opérations les unes après les autres
		{
			
			tt=eeprom_read_byte(&ee_trans.tt[i]);   // récupération de la taile du transfert i
		
			p_dst=(uint8_t*)eeprom_read_word((uint16_t*)&ee_trans.p_dst[i]); // récupération du pointeur de l'adresse de destination du transfert i
			// transfert effectif: les données de la sources sont recopiées dans la destination
			for(j=0;j<tt;j++)
			{
				eeprom_write_byte(p_dst++,eeprom_read_byte(p_src++));   // les incréments d'adresse servent à déplacer les "têtes de lecture" à chaque passage de la boucle
			}
		}
	}
	eeprom_write_byte((uint8_t*)&ee_trans.state,vide); // une fois le transfert des données terminé, opn repasse le statut à vide, la validation est terminée
}


//########################################################################################################################
//########################################           ENGAGEMENT              #############################################
//########################################################################################################################

// engagement d'une transaction
// appel de la forme engage(n1, p_src1, p_dst1, n2, p_src2, p_dst2, ... 0)
// ni : taille des données à transférer
// p_srci : adresse des données à transférer
// p_dsti : destination des données à transférer

void engage(int tt, ...)
{
	va_list args; //variable qui va permettre de récupérer la liste des arguments passés à la fonction engage
	uint8_t nb_ope;
	uint8_t*p_src;
	uint8_t*p_buf;

	
	eeprom_write_byte((uint8_t*)&ee_trans.state,vide);  // avant de commencer réellement les opérations, on passe le statut du transfert à vide

	va_start(args,tt); //initialisation de la liste d'arguments sur le premier argument
	nb_ope=0;
	p_buf=ee_trans.buffer; //récupération de l'adresse du buffer
	while(tt!=0) //cette condition dans la boucle while explique pourquoi le dernier argument passé à la fonction doit être 0
	{
		// Lecture des différents arguments
		p_src=va_arg(args,uint8_t*);//Récupération du 3k+1 ième argument qui est l'adresse de la source de la k ième opération, donc un pointeur sur uint8_t
		
		eeprom_write_block(p_src,p_buf,tt); //écriture du contenu de la source dans le buffer, taille d'écriture : tt
		
		
		p_buf+=tt; //incrémentation du pointeur sur le buffer pour pouvoir écrire à la suite
		// le deuxième paramètre récupère le 3k+2 ième argument qui contient l'adresse de destination de la source de la k ième opération. Cette adresse est ensuite écrite dans la table des destinations de ee_trans
		eeprom_write_word((uint16_t*)&ee_trans.p_dst[nb_ope],(uint16_t)va_arg(args,uint8_t*));
		//écriture de la taille de données à transférer dans le tableau des taille de transfert
		eeprom_write_byte(&ee_trans.tt[nb_ope],tt);
		nb_ope++; // Incrémentation du nombre d'opération
		tt=va_arg(args,int);	// taille suivante dans la liste
	}
	// écriture du nombre de transactions
	eeprom_write_byte(&ee_trans.nb_ope,nb_ope);
	va_end(args);
	
	eeprom_write_byte((uint8_t*)&ee_trans.state,plein); // engagement terminé, on passe le statut à plein en l'écrivant dans ee_trans.state
}

//======================================================================================

//########################################################################################################################
//############################################      PERSONNALISATION      ################################################
//########################################################################################################################
//déclaration du propriétaire dans l'EEPROM
char proprietaire[20] EEMEM;
uint8_t sizeProp EEMEM;


void introOwner()
{
    	int i;
     	// vérification de la taille
    	if (p3>20)
	{
	   	sw1=0x6c;	// P3 incorrect
        	sw2=20;	// sw2 contient l'information de la taille correcte
		return;
    	}
	sendbytet0(ins);	// acquitement

	for(i=0;i<p3;i++)	// boucle d'envoi du message
	{
	    data[i]=recbytet0();
	}
	//écriture en eeprom du propriétaire
	//eeprom_write_block(data,proprietaire,p3);
	//eeprom_write_byte(&sizeProp,p3);
	engage(p3,data,proprietaire,1,&p3,&sizeProp,0);
	taille=p3; 		// mémorisation de la taille des données lues
	sw1=0x90;
	valide();
}

//====================
// lecture perso
void showOwner(){
	uint8_t size;
	size=eeprom_read_byte(&sizeProp);
	if (size==0){
		sw1=0x61;
		sw2=0;
		return;
	}
	if (p3!=size)
    	{
        	if(p3>MAXI){
        		sw1=0x6c;	// taille incorrecte
			sw2=MAXI;		// taille attendue
			return;
        	}
        	sw1=0x6c;	// taille incorrecte
        	sw2=size;		// taille attendue
        	return;
    	}
	sendbytet0(ins);
	char lecture[p3];
	eeprom_read_block(lecture,proprietaire,p3);
	for(int i=0;i<p3;i++){
		sendbytet0(lecture[i]);
	}
	taille = p3;
	sw1=0x90;
}
//########################################################################################################################
//############################################       GESTION DU SOLDE     ################################################
//########################################################################################################################



//Déclaration de variables eeprom pour le solde
//initialisation du solde chiffré
uint32_t solde1 EEMEM=0x9c45df56;
uint32_t solde2 EEMEM=0x7194cb80;


void recupererSolde(uint32_t *clef1, uint16_t *destination){//cette fonction est implémentée à part car elle va servir dans lireSolde, dans credit et dans debit. Cela allège le code.
	uint32_t s[2]={0,0};
	for(int i=0;i<4;i++){ //lecture de solde1
		s[0]+=eeprom_read_byte((uint8_t*) &solde1 + 3 - i);
		if(i!=3){
			s[0]<<=8;
		}
	}
	for(int i=0;i<4;i++){ //lecture de solde2
		s[1]+=eeprom_read_byte((uint8_t*) &solde2 + 3 - i);
		if(i!=3){
			s[1]<<=8;
		}
	}
	uint32_t cleCode[2]={0,0};
	tea_dechiffre(s,cleCode,clef1);
	*((uint8_t*)(destination))=(*((uint8_t*)cleCode));
	*(((uint8_t*)destination) + 1 )=(*((uint8_t*)cleCode + 1));
}


// lecture du solde
void lireSolde(uint32_t *k){
	if (p3!=2)
    	{
        	sw1=0x6c;	// taille incorrecte
        	sw2=2;		// taille attendue
        	return;
    	}
    	sendbytet0(ins);

    	
    	uint16_t soldeLu=0;
    	recupererSolde(k,&soldeLu);

	//Fin lecture, ici le solde est en clair
    	sendbytet0(*((uint8_t*)&soldeLu + 1));
    	sendbytet0(*((uint8_t*)&soldeLu));
    	taille = p3;
    	sw1=0x90;
    	sw2=0;
}
// crédit
void crediter(uint32_t *key1){ //testée, se comporte comporte correctement
	if(p3!=2){
		sw1=0x6c; //taille incorrecte
		sw2=2;
		return;
	}
	sendbytet0(ins);	
	uint8_t data[2];
	for(int i=0;i<p3;i++)	// boucle d'envoi du message
	{
		//récupération des octets du solde à créditer
		data[i]=recbytet0();
	}
	
	
//################################ Récupération du solde chiffré
	uint16_t soldeActuel;
	recupererSolde(key1,&soldeActuel);

	uint16_t soldeACrediter=0;//=(uint16_t)(data[1])+((uint16_t)(data[0])<<8);
	*((uint8_t*)&soldeACrediter)=data[1];
	*((uint8_t*)&soldeACrediter +1)=data[0];
	if(soldeACrediter>(0xffff-soldeActuel)){
		//le solde max va être dépassé
		//code erreur: out of boundary
		sw1=0x91;
		sw2=0xBE; 
		return;
	}
	uint16_t nouveauSolde=soldeActuel+soldeACrediter;
	uint32_t soldeCodeInter=(((uint32_t)nouveauSolde)<<16)+((uint32_t)nouveauSolde);
	uint32_t soldeCode[2]={soldeCodeInter,soldeCodeInter};
	uint32_t nouveauSoldeChiffre[2]={0,0};
	tea_chiffre(soldeCode,nouveauSoldeChiffre,key1);
	//engagement de l'opération d'écriture du nouveau solde
	engage(4,(uint8_t*)nouveauSoldeChiffre,&solde1,4,(uint8_t*)nouveauSoldeChiffre + 4,&solde2,0);
	taille=p3; 		// mémorisation de la taille des données lues
	sw1=0x90;
	//validation de la transaction
	valide();
	
}
// débit
void debiter(uint32_t* clef1){
	if(p3!=2){
		sw1=0x6c; //taille incorrecte
		sw2=2;
		return;
	}
	sendbytet0(ins);
	for(int i=0;i<p3;i++){	// boucle d'envoi du message
		//récupération des octets du solde à créditer
		data[i]=recbytet0();
	}
//################################ Récupération du solde chiffré	
	uint16_t soldeActuel;
	recupererSolde(clef1,&soldeActuel);


	
	
	uint16_t soldeADebiter=0;
	*((uint8_t*)&soldeADebiter)=data[1];
	*((uint8_t*)&soldeADebiter +1)=data[0];
	if(soldeADebiter>soldeActuel){
		//le solde est insuffisant
		sw1=0x91;
		sw2=0xBE;
		return;
	}
	uint16_t nouveauSolde=soldeActuel-soldeADebiter;
	uint32_t soldeCodeInter=(((uint32_t)nouveauSolde)<<16)+((uint32_t)nouveauSolde);
	uint32_t soldeCode[2]={soldeCodeInter,soldeCodeInter};
	uint32_t nouveauSoldeChiffre[2]={0,0};
	tea_chiffre(soldeCode,nouveauSoldeChiffre,clef1);
	//engagement de l'opération d'écriture du nouveau solde
	engage(4,(uint8_t*)nouveauSoldeChiffre,&solde1,4,(uint8_t*)nouveauSoldeChiffre + 4,&solde2,0);
	taille=p3; 		// mémorisation de la taille des données lues
	sw1=0x90;
	//validation de la transaction
	valide();
}


//########################################################################################################################
//###########################################      GESTION DE LA CLEF      ###############################################
//########################################################################################################################
uint8_t compteurEssai EEMEM=3; //nombre d'essai autorisé pour entrer une clef
void introClef(uint32_t *cle){
	if(p3!=16){
		sw1=0x6c;
		sw2=16;
		return;
	}
	sendbytet0(ins);   //acquittement
	
	//récupération de la clé
	uint8_t buff[16];
	for(int i=0;i<16;i++){
		buff[i]=recbytet0();
	}
	for(int i=0;i<4;i++){
		for(int j=0;j<4;j++){
			*( ((uint8_t*)cle) + 4*i + 3 - j)=buff[4*i+j];
		}
	}
	sw1=0x90;
	sw2=0;	
}

int testClef(uint32_t *clef){ //affichage de la clé pour l'instant
	if(p3!=2){
		sw1=0x6c;
		sw2=2;
		return 0;
	}
	sendbytet0(ins);   //acquittement
	
	//récupération solde chiffré : testé stocké en little endian, correspond bien +++
	uint32_t s[2]={0,0};
	for(int i=0;i<4;i++){ //lecture de solde1
		s[0]+=eeprom_read_byte((uint8_t*) &solde1 + 3 - i);
		if(i!=3){
			s[0]<<=8;
		}
	}
	for(int i=0;i<4;i++){ //lecture de solde2
		s[1]+=eeprom_read_byte((uint8_t*) &solde2 + 3 - i);
		if(i!=3){
			s[1]<<=8;
		}
	}
	uint32_t code[2]={0,0};
	tea_dechiffre(s,code,clef); // fonctionne correctement, renvoie bien le solde déchiffré (format code initial 0x00640064 0x00640064 stocké en little endian)
	int rep=1;
	int cmptTmp=eeprom_read_byte(&compteurEssai);//récupération du compteur d'essai
	//On va vérifier si le résultat du déchiffrement est bien de la forme : {0xABCDABCD;0xABCDABCD}
	if(code[0]!=code[1]){ //Si le code est erroné, on décrémente le compteur et on passe la valeur de retour à 0
		rep=0;
		cmptTmp--;		
	}else{ //ici on a l'égalité entre code[0] et code[1], nous allons donc vérifier si code[0] est de la forme 0xABCDABCD (pas besoin de faire la même chose avec code[1] puisque code[1]=code[0] dans cette partie du test)
		if((code[0]>>16)!=(code[0]&0x0000ffff)){ //Si le code est erroné, on décrémente le compteur et on passe la valeur de retour à 0
			rep=0;
			cmptTmp--;
		}else{//Si le code est valide, on repasse le compteur à 3
			cmptTmp=3;
		}	
	}
	engage(1,&cmptTmp,&compteurEssai,0);
	valide();
	
	//on envoie le résultat du test puis le nombre d'essais restant
	sendbytet0(rep);
	sendbytet0(cmptTmp);
	sw1=0x90;
	sw2=0;	
	return rep;
}

// Programme principal
//====================
int main(void)
{
  	// initialisation des ports
	ACSR=0x80;
	DDRB=0xff;
	DDRC=0xff;
	DDRD=0;
	PORTB=0xff;
	PORTC=0xff;
	PORTD=0xff;
	ASSR=1<<EXCLK;
	TCCR2A=0;
	ASSR|=1<<AS2;


	// ATR
  	atr();

	taille=0;
	sw2=0;		// pour éviter de le répéter dans toutes les commandes
  	// boucle de traitement des commandes
  	
  	uint32_t key[4]={0,0,0,0}; //déclaration de la clé en RAM
  	
  	
	int verrou=0;
	


	int compteurTempo=0;
	
  	for(;;)
  	{
  		compteurTempo=eeprom_read_byte(&compteurEssai);
  		if(compteurTempo==0){break;} //Si le nombre d'essai max a été atteint, on empèche toute intéraction avec la carte, elle est bloquée
  		
    		// lecture de l'entête
    		cla=recbytet0();
    		ins=recbytet0();
    		p1=recbytet0();
	    	p2=recbytet0();
    		p3=recbytet0();
	    	sw2=0;
	    	
	    	
		switch (cla)
		{
	  	case 0x80:
		    	switch(ins)
			{
			case 0:
				version();
				break;
			
			case 1:
				introClef(key);
				break;
			case 2:
				verrou=testClef(key);
				break;
			
	    		default:
		    		sw1=0x6d; // code erreur ins inconnu
		    		break;
			}
			break;
		case 0x81:
			if(verrou==0){break;} //on empèche d'interagir avec la carte si le verrou n'est pas levé
			switch(ins)
			{
			case 2:
				introOwner();
				break;
			case 3:
				showOwner();
				break;
			case 4:
				lireSolde(key);
				break;
			case 5:
				crediter(key);
				break;
			case 6:
				debiter(key);
				break;
			default:
				sw1=0x6d; // code erreur ins inconnu
				break;
			}
			break;
      		default:
			sw1=0x6e; // code erreur classe inconnue
		}
		if(verrou==0 && cla!=0x80){break;};// si l'utilisateur a tenté d'intéragir avec la carte sans entrer la bonne clé, on sort de la boucle et on arrête ainsi le programme
		sendbytet0(sw1); // envoi du status word
		sendbytet0(sw2);
  	}
	//}
  	return 0;
}

